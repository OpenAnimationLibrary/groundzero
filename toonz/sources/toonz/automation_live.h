#pragma once

#include "automation.h"

#include <functional>
#include <memory>

namespace GroundzeroAutomation {
namespace Live {

struct Options final {
  QString mode;
  QString sessionPath;
};

// The GUI supplies current native history, producer, tool, and lifecycle
// guards.
using BusyProbe = std::function<QStringList()>;

// Construction and every method run on the GUI thread. Start only after the
// normal editor has entered its outer event loop; destruction revokes the
// grant.
class Service final {
public:
  Service(Options options, BusyProbe busyProbe);
  ~Service();
  Service(const Service&)            = delete;
  Service& operator=(const Service&) = delete;
  void start();
  void stop() noexcept;
  bool running() const;

private:
  class Impl;
  std::unique_ptr<Impl> m_impl;
};

QJsonObject limits();
QJsonArray operations();

// SceneState is app-owned and GUI-only. Its implementation retains native
// leases and identities without transferring the editor scene to a Document.
class SceneState;
std::shared_ptr<SceneState> createSceneState(const QString& instanceId);
QJsonObject refreshScene(SceneState& state);
QJsonObject inspectScene(SceneState& state, const QJsonObject& params);
QJsonObject editStage(SceneState& state, const QJsonObject& params);
QJsonObject editFx(SceneState& state, const QJsonObject& params);
void resetMutationStarted(SceneState& state);
bool mutationStarted(const SceneState& state);

}  // namespace Live
}  // namespace GroundzeroAutomation
