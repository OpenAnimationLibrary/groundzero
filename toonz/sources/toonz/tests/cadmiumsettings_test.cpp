#include "cadmiumruntime.h"
#include "cadmiumsettingswidget.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QLineEdit>
#include <QMap>
#include <QProcess>
#include <QSettings>
#include <QSpinBox>
#include <QTemporaryDir>
#include <iostream>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

// Keep the executable independent of a developer's saved settings, including
// platforms where native settings are not stored under a filesystem path.
class SavedPreferences {
  QMap<QString, QVariant> m_saved;
public:
  SavedPreferences() {
    QSettings settings(QStringLiteral("OpenToonz"), QStringLiteral("Cadmium"));
    for (const QString &key : settings.allKeys()) m_saved[key] = settings.value(key);
    settings.clear(); settings.sync();
  }
  ~SavedPreferences() {
    QSettings settings(QStringLiteral("OpenToonz"), QStringLiteral("Cadmium"));
    settings.clear();
    for (auto i = m_saved.constBegin(); i != m_saved.constEnd(); ++i)
      settings.setValue(i.key(), i.value());
    settings.sync();
  }
};
}

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
  QApplication app(argc, argv);
  SavedPreferences saved;
  auto *runtime = CadmiumRuntime::instance();
  try {
    require(runtime->startupMode() == CadmiumRuntime::OnDemand,
            "on-demand startup is the fresh-install default");
    require(!runtime->busy() && !runtime->isReady(),
            "constructing runtime does not load models");
    CadmiumSettingsWidget preferences;
    CadmiumSettingsWidget startup(nullptr, true);
    auto *prefMode = preferences.findChild<QComboBox *>("cadmiumStartupMode");
    auto *startMode = startup.findChild<QComboBox *>("cadmiumStartupMode");
    require(prefMode && startMode, "startup and preferences expose mode controls");
    require(prefMode->count() == 3 && startMode->count() == 3,
            "both screens expose all startup choices");
    prefMode->setCurrentIndex(CadmiumRuntime::Off);
    require(runtime->startupMode() == CadmiumRuntime::Off &&
                startMode->currentIndex() == CadmiumRuntime::Off,
            "preference changes synchronize startup without recursion");

    int failures = 0, results = 0;
    QObject::connect(runtime, &CadmiumRuntime::failed, &app,
                     [&failures](const QString &) { ++failures; });
    QObject::connect(runtime, &CadmiumRuntime::resultReady, &app,
                     [&results](const QJsonObject &) { ++results; });
    runtime->colorize(QJsonObject{{"unused", true}});
    require(failures == 1 && results == 0 && !runtime->busy(),
            "off refuses requests without queuing artwork");
    for (QProcess *process : runtime->findChildren<QProcess *>())
      require(process->state() == QProcess::NotRunning,
              "off starts no worker process");

    QTemporaryDir directory;
    require(directory.isValid(), "temporary setup directory available");
    const QString missingWorker = QDir(directory.path()).filePath("missing-worker");
    runtime->setExecutablePath(missingWorker);
    runtime->setModelsDirectory(directory.path());
    auto *executable = preferences.findChild<QLineEdit *>("cadmiumExecutablePath");
    auto *models = preferences.findChild<QLineEdit *>("cadmiumModelsDirectory");
    require(executable && executable->text() == missingWorker &&
                models && models->text() == directory.path(),
            "setup selections synchronize visible fields");
    startMode->setCurrentIndex(CadmiumRuntime::Preload);
    require(prefMode->currentIndex() == CadmiumRuntime::Preload,
            "startup changes synchronize preferences");
    require(failures == 2 && !runtime->isReady() && !runtime->busy(),
            "missing worker is a recoverable preload failure");
    QSettings persisted(QStringLiteral("OpenToonz"), QStringLiteral("Cadmium"));
    require(persisted.value("startupMode").toInt() == CadmiumRuntime::Preload,
            "startup choice persists without manual file edits");
    require(persisted.value("executable").toString() == missingWorker,
            "runtime setup persists");
    auto *idle = preferences.findChild<QSpinBox *>("cadmiumIdleUnloadMinutes");
    require(idle, "idle unload has a visible control");
    idle->setValue(7);
    require(runtime->idleUnloadMinutes() == 7, "idle control updates runtime");
    runtime->setIdleUnloadMinutes(12);
    require(idle->value() == 12, "external idle changes synchronize control");
    runtime->setStartupMode(CadmiumRuntime::OnDemand);
    require(!runtime->busy() && !runtime->isReady(),
            "on-demand selection does not start loading");
    runtime->setStartupMode(99);
    require(runtime->startupMode() == CadmiumRuntime::OnDemand,
            "invalid mode does not replace remembered choice");
    runtime->stop();
    require(!runtime->busy() && !runtime->isReady(), "unload is safe after failed setup");
    std::cout << "cadmiumsettings passed\n";
    return 0;
  } catch (const std::exception &error) {
    runtime->stop();
    std::cerr << error.what() << '\n';
    return 1;
  }
}
