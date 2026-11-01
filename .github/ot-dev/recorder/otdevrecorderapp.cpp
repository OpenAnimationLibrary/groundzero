#include "otdevrecorder.h"

#include "toonzqt/gutil.h"
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
  auto useIcon = [](QIcon &icon, const QString &name) {
    const QIcon themed = createQIcon(name);
    if (!themed.isNull()) icon = themed;
  };
  useIcon(m_recordIcon, "record");
  useIcon(m_stopIcon, "stop");
  useIcon(m_pauseIcon, "pause");
  useIcon(m_resumeIcon, "play");
  useIcon(m_settingsIcon, "gear");
  m_start->setIcon(m_recordIcon);
  m_stop->setIcon(m_stopIcon);
  m_settingsAction->setIcon(m_settingsIcon);
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
