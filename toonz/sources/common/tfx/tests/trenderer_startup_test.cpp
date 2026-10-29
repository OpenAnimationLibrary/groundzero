// SPDX-License-Identifier: BSD-3-Clause

#include "trenderer.h"
#include "tthread.h"
#include "texception.h"

#include <QCoreApplication>
#include <QEventLoop>
#include <QTimer>

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
    std::cout << "Renderer startup completion passed\n";
    return 0;
  } catch (const TException &) {
    std::cerr << "Unexpected renderer exception\n";
  } catch (const std::exception &error) {
    std::cerr << error.what() << '\n';
  }
  return 1;
}
