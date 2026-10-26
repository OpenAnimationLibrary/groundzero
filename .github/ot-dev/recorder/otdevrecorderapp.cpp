#include "otdevrecorder.h"

#include "toonzqt/menubarcommand.h"

namespace {
class RecordingCommand final : public CommandHandlerInterface {
  QPointer<QAction> m_action;

 public:
  explicit RecordingCommand(QAction *action) : m_action(action) {}
  void execute() override {
    if (m_action && m_action->isEnabled()) m_action->trigger();
  }
};
}  // namespace

void OtDevRecorder::registerCommands() {
  auto *manager = CommandManager::instance();
  for (auto *action : m_commands) {
    const QByteArray id = action->objectName().toUtf8();
    const CommandType type =
        action == m_togglePanel ? MenuWindowsCommandType : MiscCommandType;
    manager->define(id.constData(), type, "", action);
    manager->setHandler(id.constData(), new RecordingCommand(action));
  }
  // MainWindow loads shortcuts before the CI-only recorder is constructed.
  manager->loadShortcuts();
  updateStatus(tr("Recording off"));
}
