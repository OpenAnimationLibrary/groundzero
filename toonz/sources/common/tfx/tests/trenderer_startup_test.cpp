// SPDX-License-Identifier: BSD-3-Clause

#include "trenderer.h"
#include "tthread.h"
#include "texception.h"
#include "tsystem.h"
#include "ttile.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QMutex>
#include <QMutexLocker>
#include <QSemaphore>
#include <QTimer>

#include <algorithm>
#include <array>
#include <atomic>
#include <iostream>
#include <stdexcept>

namespace {

void require(bool value, const char *message) {
  if (!value) throw std::runtime_error(message);
}

enum class StartupAction { CancelAlias, CancelFirstRun, CancelTestRun,
                           FailAlias, FailPrecompute, FailStartHandler };

class StartupFx final : public TRasterFx {
  TRenderer &m_renderer;
  StartupAction m_action;
  mutable int m_aliasCalls = 0;
  int m_dryCalls = 0;

public:
  int computed = 0;
  int ended = 0;

  StartupFx(TRenderer &renderer, StartupAction action)
      : m_renderer(renderer), m_action(action) {}

  std::string getPluginId() const override { return "startup-test"; }
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TRectD(0, 0, 4, 4);
    return true;
  }
  void doCompute(TTile &, double, const TRenderSettings &) override {
    ++computed;
  }
  std::string getAlias(double frame, const TRenderSettings &) const override {
    ++m_aliasCalls;
    if (m_action == StartupAction::FailAlias)
      throw TException("Intentional alias failure");
    if (m_action == StartupAction::CancelAlias && m_aliasCalls == 1)
      m_renderer.stopRendering(false);
    return "startup-test-" + std::to_string(frame);
  }
  void dryCompute(TRectD &, double, const TRenderSettings &) override {
    ++m_dryCalls;
    if (m_action == StartupAction::FailPrecompute)
      throw std::runtime_error("Intentional precompute failure");
    if ((m_action == StartupAction::CancelFirstRun && m_dryCalls == 1) ||
        (m_action == StartupAction::CancelTestRun && m_dryCalls == 3))
      m_renderer.stopRendering(false);
  }
  void callStartRenderHandler() override {
    if (m_action == StartupAction::FailStartHandler)
      throw TException("Intentional start handler failure");
  }
  void callEndRenderHandler() override { ++ended; }
};

class StartupPort final : public TRenderPort {
public:
  QEventLoop loop;
  int finished = 0;
  int failed = 0;
  int completed = 0;
  bool canceled = false;

  void onRenderFailure(const RenderData &data, TException &) override {
    require(data.m_frames.size() == 2,
            "startup failure omitted requested frames");
    ++failed;
  }
  void onRenderRasterCompleted(const RenderData &) override { ++completed; }
  void onRenderFinished(bool value) override {
    ++finished;
    canceled = value;
    loop.quit();
  }
};

void checkStartup(StartupAction action, bool canceled) {
  TRenderer renderer(1);
  renderer.enablePrecomputing(true);
  renderer.enableFrameStrips(false);
  TRasterFxP fx = new StartupFx(renderer, action);
  StartupFx *fixture = static_cast<StartupFx *>(fx.getPointer());
  StartupPort port;
  port.setRenderArea(TRectD(0, 0, 4, 4));
  renderer.addPort(&port);
  TFxPair roots;
  roots.m_frameA = fx;
  TRenderSettings settings;
  auto *frames = new RenderDataVector;
  frames->push_back(TRenderer::RenderData(0, settings, roots));
  frames->push_back(TRenderer::RenderData(1, settings, roots));
  renderer.startRendering(frames);

  bool timedOut = false;
  QTimer timeout;
  timeout.setSingleShot(true);
  QObject::connect(&timeout, &QTimer::timeout, [&] {
    timedOut = true;
    port.loop.quit();
  });
  timeout.start(5000);
  port.loop.exec();
  timeout.stop();
  QCoreApplication::processEvents();
  renderer.removePort(&port);
  require(!timedOut, "startup did not send terminal notification");
  require(port.finished == 1, "startup sent duplicate terminal notifications");
  require(port.canceled == canceled, "startup reported the wrong terminal state");
  require(port.failed == (canceled ? 0 : 1),
          "startup failure was not delivered exactly once");
  require(port.completed == 0 && fixture->computed == 0,
          "canceled or failed startup dispatched rendering tasks");
  require(fixture->ended == 1, "startup did not finish the effect lifecycle");
}

struct WindowOptions {
  std::uint64_t budget = 0;
  int windowFrames = 0;
  int threads = 1;
  bool pairedAliases = false;
  bool gateFirst = false;
  bool cancel = false;
  int failFxFrame = -1;
  int failPortFrame = -1;
};

struct WindowResults {
  std::vector<std::vector<int>> clusters;
  std::vector<int> completionOrder;
  std::array<int, 6> starts{};
  std::array<int, 6> attempts{};
  std::array<int, 6> completed{};
  std::array<int, 6> failed{};
  int finished = 0;
  bool canceled = false;
  bool pixelsValid = true;
  bool framesValid = true;
  bool admissionValid = true;
};

struct WindowState {
  QMutex mutex;
  WindowResults results;
  QSemaphore firstGate;
  std::atomic<bool> firstEntered{false};
  std::atomic<bool> secondCallbackReturned{false};
  std::atomic<bool> gateHeld{false};
  std::atomic<bool> futureStartedWhileGated{false};
  std::atomic<int> fxStarted{0};
  std::atomic<int> fxEnded{0};
};

TPixel32 windowPixel(int frame) {
  return frame % 2 ? TPixel32::Blue : TPixel32::Red;
}

class WindowFx final : public TRasterFx {
  WindowState &m_state;
  const WindowOptions &m_options;
  std::string m_prefix;

public:
  WindowFx(WindowState &state, const WindowOptions &options)
      : m_state(state), m_options(options) {
    static int sequence = 0;
    // Failure and gate cases must compute their own pixels, even if a previous
    // test left identically sized output in a shared FX cache.
    m_prefix = "output-window-test-" + std::to_string(++sequence) + "-";
  }

  std::string getPluginId() const override { return "output-window-test"; }
  const TPersistDeclaration *getDeclaration() const override { return nullptr; }
  bool canHandle(const TRenderSettings &, double) override { return true; }
  bool doGetBBox(double, TRectD &box, const TRenderSettings &) override {
    box = TRectD(0, 0, 4, 4);
    return true;
  }
  std::string getAlias(double frame, const TRenderSettings &) const override {
    const int key = m_options.pairedAliases ? int(frame) % 2 : int(frame);
    return m_prefix + std::to_string(key);
  }
  void doCompute(TTile &tile, double frame,
                 const TRenderSettings &) override {
    if (m_options.gateFirst && frame == 0) {
      m_state.firstEntered = true;
      m_state.firstGate.acquire();
    }
    if (frame == m_options.failFxFrame)
      throw TException("Intentional output-window FX failure");
    TRaster32P raster = tile.getRaster();
    if (!raster) throw TException("Output-window fixture requires RGBA pixels");
    raster->fill(windowPixel(int(frame)));
  }
  void callStartRenderHandler() override { ++m_state.fxStarted; }
  void callEndRenderHandler() override { ++m_state.fxEnded; }
  void callEndRenderFrameHandler(const TRenderSettings *, double frame) override {
    // RenderTask invokes this after the port completion callback returns.
    if (frame == 1) m_state.secondCallbackReturned = true;
  }
};

class WindowPort final : public TRenderPort {
  WindowState &m_state;
  const WindowOptions &m_options;

  std::vector<int> frames(const RenderData &data) {
    std::vector<int> result;
    for (double value : data.m_frames) {
      const int frame = int(value);
      if (frame < 0 || frame >= 6 || value != frame)
        m_state.results.framesValid = false;
      else
        result.push_back(frame);
    }
    if (result.empty()) m_state.results.framesValid = false;
    return result;
  }

public:
  QEventLoop loop;

  WindowPort(WindowState &state, const WindowOptions &options)
      : m_state(state), m_options(options) {
    setRenderArea(TRectD(0, 0, 4, 4));
  }
  void onRenderRasterStarted(const RenderData &data) override {
    QMutexLocker locker(&m_state.mutex);
    const auto cluster = frames(data);
    m_state.results.clusters.push_back(cluster);
    for (int frame : cluster) {
      ++m_state.results.starts[frame];
      if (frame >= 2 && m_state.gateHeld)
        m_state.futureStartedWhileGated = true;
      if (m_options.windowFrames > 0) {
        const int windowStart =
            (frame / m_options.windowFrames) * m_options.windowFrames;
        for (int earlier = 0; earlier < windowStart; ++earlier)
          if (m_state.results.completed[earlier] +
                  m_state.results.failed[earlier] !=
              1)
            m_state.results.admissionValid = false;
      }
    }
  }
  void onRenderRasterCompleted(const RenderData &data) override {
    QMutexLocker locker(&m_state.mutex);
    const auto cluster = frames(data);
    for (int frame : cluster) ++m_state.results.attempts[frame];
    if (std::find(cluster.begin(), cluster.end(), m_options.failPortFrame) !=
        cluster.end())
      throw TException("Intentional output-window completion callback failure");

    TRaster32P raster = data.m_rasA;
    if (!raster || raster->getLx() != 4 || raster->getLy() != 4 || data.m_rasB)
      m_state.results.pixelsValid = false;
    else {
      raster->lock();
      for (int frame : cluster)
        for (int y = 0; y < raster->getLy(); ++y)
          for (int x = 0; x < raster->getLx(); ++x)
            if (raster->pixels(y)[x] != windowPixel(frame))
              m_state.results.pixelsValid = false;
      raster->unlock();
    }
    for (int frame : cluster) {
      ++m_state.results.completed[frame];
      m_state.results.completionOrder.push_back(frame);
    }
  }
  void onRenderFailure(const RenderData &data, TException &) override {
    QMutexLocker locker(&m_state.mutex);
    for (int frame : frames(data)) ++m_state.results.failed[frame];
  }
  void onRenderFinished(bool canceled) override {
    {
      QMutexLocker locker(&m_state.mutex);
      ++m_state.results.finished;
      m_state.results.canceled = canceled;
    }
    loop.quit();
  }
};

WindowResults renderWindows(const WindowOptions &options) {
  TRenderer renderer(options.threads);
  renderer.enablePrecomputing(false);
  renderer.enableFrameStrips(false);
  renderer.setOutputRasterBudget(options.budget);
  WindowState state;
  state.gateHeld = options.gateFirst;
  TRasterFxP fx = new WindowFx(state, options);
  WindowPort port(state, options);
  renderer.addPort(&port);

  bool timedOut = false, gateReleased = false;
  const auto releaseGate = [&] {
    state.gateHeld = false;
    state.firstGate.release();
    gateReleased = true;
  };

  QTimer control, release, timeout;
  control.setInterval(1);
  release.setSingleShot(true);
  timeout.setSingleShot(true);
  QObject::connect(&release, &QTimer::timeout, releaseGate);
  QObject::connect(&control, &QTimer::timeout, [&] {
    if (!state.firstEntered) return;
    if (options.cancel) {
      control.stop();
      renderer.stopRendering(false);
      releaseGate();
    } else if (state.secondCallbackReturned) {
      control.stop();
      // Keep frame 0 blocked while the controller processes frame 1's finished
      // slot and has an opportunity to incorrectly admit a later window.
      release.start(25);
    }
  });
  QObject::connect(&timeout, &QTimer::timeout, [&] {
    timedOut = true;
    releaseGate();
    renderer.stopRendering(false);
    port.loop.quit();
  });

  TFxPair roots;
  roots.m_frameA = fx;
  TRenderSettings settings;
  settings.m_bpp = 32;
  auto *request = new RenderDataVector;
  for (int frame = 0; frame < 6; ++frame)
    request->push_back(TRenderer::RenderData(frame, settings, roots));
  renderer.startRendering(request);
  if (options.gateFirst) control.start();
  timeout.start(5000);
  port.loop.exec();
  control.stop();
  release.stop();
  timeout.stop();
  const bool gateWasReleased = gateReleased;
  // Even a failing test releases its worker gate before waiting for shutdown.
  releaseGate();
  renderer.stopRendering(true);
  QCoreApplication::processEvents();
  renderer.removePort(&port);

  require(!timedOut, "output-window render did not complete before timeout");
  require(state.fxStarted == 1 && state.fxEnded == 1,
          "output windows split or omitted the FX render lifecycle");
  require(!state.futureStartedWhileGated,
          "later output window started while the first frame was blocked");
  if (options.gateFirst)
    require(gateWasReleased && state.firstEntered,
            "output-window fixture did not exercise its active worker gate");
  const WindowResults results = state.results;
  require(results.finished == 1, "output windows sent duplicate terminal events");
  require(results.canceled == options.cancel,
          "output windows reported the wrong cancellation state");
  require(results.framesValid && results.pixelsValid,
          "output windows returned incorrect frames or pixel colors");
  require(results.admissionValid,
          "window started before every earlier exposure completed or failed");
  return results;
}

void checkSuccessfulWindows(std::uint64_t budget, int windowFrames,
                            std::vector<std::vector<int>> expected) {
  WindowOptions options;
  options.budget = budget;
  options.windowFrames = windowFrames;
  options.pairedAliases = true;
  WindowResults result = renderWindows(options);
  std::sort(result.clusters.begin(), result.clusters.end());
  std::sort(expected.begin(), expected.end());
  require(result.clusters == expected,
          "alias clusters crossed an output-window boundary");
  for (int frame = 0; frame < 6; ++frame)
    require(result.starts[frame] == 1 && result.attempts[frame] == 1 &&
                result.completed[frame] == 1 && result.failed[frame] == 0,
            "output window lost or repeated a successful exposure");
}

void checkWindowFailure(bool callbackFailure) {
  WindowOptions options;
  options.budget = 256;
  options.windowFrames = 2;
  if (callbackFailure)
    options.failPortFrame = 1;
  else
    options.failFxFrame = 1;
  const WindowResults result = renderWindows(options);
  for (int frame = 0; frame < 6; ++frame) {
    const bool failed = frame == 1;
    require(result.starts[frame] == 1 &&
                result.failed[frame] == (failed ? 1 : 0) &&
                result.completed[frame] == (failed ? 0 : 1),
            "frame failure suppressed a result or stranded a later window");
    require(result.attempts[frame] == (failed && !callbackFailure ? 0 : 1),
            "completion callback was omitted or repeated after a frame failure");
  }
}

void checkGatedWindows(bool cancel, bool parallel) {
  WindowOptions options;
  options.budget = parallel ? 256 : 128;
  options.windowFrames = parallel ? 2 : 1;
  options.threads = parallel ? 2 : 1;
  options.gateFirst = true;
  options.cancel = cancel;
  const WindowResults result = renderWindows(options);
  if (cancel) {
    for (int frame = 0; frame < options.windowFrames; ++frame)
      require(result.completed[frame] + result.failed[frame] == 1,
              "cancellation omitted an admitted frame's terminal callback");
    for (int frame = parallel ? 2 : 1; frame < 6; ++frame)
      require(result.starts[frame] == 0 && result.attempts[frame] == 0 &&
                  result.completed[frame] == 0 && result.failed[frame] == 0,
              "cancellation admitted a future output window");
  } else {
    require(result.completionOrder.size() == 6 &&
                result.completionOrder[0] == 1,
            "slow-first fixture did not force out-of-order completion");
    for (int frame = 0; frame < 6; ++frame)
      require(result.starts[frame] == 1 && result.attempts[frame] == 1 &&
                  result.completed[frame] == 1 && result.failed[frame] == 0,
              "slow-first output window lost or repeated an exposure");
  }
}

void checkOutputWindows() {
  // A 4 x 4 RGBA output plus its handoff clone costs 128 bytes per exposure.
  checkSuccessfulWindows(0, 0, {{0, 2, 4}, {1, 3, 5}});
  checkSuccessfulWindows(384, 3, {{0, 2}, {1}, {3, 5}, {4}});
  checkSuccessfulWindows(128, 1, {{0}, {1}, {2}, {3}, {4}, {5}});
  checkWindowFailure(false);
  checkWindowFailure(true);
  const bool parallel = TSystem::getProcessorCount() >= 2;
  if (parallel) checkGatedWindows(false, true);
  checkGatedWindows(true, parallel);
}

}  // namespace

int main(int argc, char **argv) {
  QCoreApplication application(argc, argv);
  try {
    TThread::init();
    TRenderer::initialize();
    checkStartup(StartupAction::CancelAlias, true);
    checkStartup(StartupAction::CancelFirstRun, true);
    checkStartup(StartupAction::CancelTestRun, true);
    checkStartup(StartupAction::FailAlias, false);
    checkStartup(StartupAction::FailPrecompute, false);
    checkStartup(StartupAction::FailStartHandler, false);
    checkOutputWindows();
    std::cout << "Renderer startup and output-window completion passed\n";
    return 0;
  } catch (const TException &) {
    std::cerr << "Unexpected renderer exception\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
