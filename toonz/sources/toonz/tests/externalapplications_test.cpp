#include "externalapplications.h"

#include <QAction>
#include <QApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLineEdit>
#include <QSettings>
#include <QTemporaryDir>
#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
void require(bool condition, const char *message) {
  if (!condition) throw std::runtime_error(message);
}

void executable(const QString &path) {
  require(QDir().mkpath(QFileInfo(path).absolutePath()),
          "fixture directory missing");
  QFile file(path);
  require(file.open(QIODevice::WriteOnly), "cannot write executable fixture");
  file.write(QByteArray(16, '\0'));
  file.close();
#ifndef Q_OS_WIN
  require(file.setPermissions(QFile::ReadOwner | QFile::WriteOwner |
                              QFile::ExeOwner),
          "cannot set fixture executable permissions");
#endif
}

struct RestoreEnvironment {
  QByteArray plugins = qgetenv("QT_PLUGIN_PATH");
  QByteArray root    = qgetenv("TOONZROOT");
  ~RestoreEnvironment() {
    if (plugins.isNull())
      qunsetenv("QT_PLUGIN_PATH");
    else
      qputenv("QT_PLUGIN_PATH", plugins);
    if (root.isNull())
      qunsetenv("TOONZROOT");
    else
      qputenv("TOONZROOT", root);
  }
};
}  // namespace

int main(int argc, char **argv) {
  qputenv("QT_QPA_PLATFORM", QByteArrayLiteral("offscreen"));
  QApplication app(argc, argv);
  try {
    QTemporaryDir directory;
    require(directory.isValid(), "temporary settings unavailable");
    QSettings settings(directory.path() + "/preferences.ini",
                       QSettings::IniFormat);
    const auto key = [](ExternalApplications::Application application) {
      return application == ExternalApplications::Ztoryc ? "ztoryc" : "ot3d";
    };
    ExternalApplications::Settings adapter{
        [&](ExternalApplications::Application application) {
          return settings.value(key(application)).toString();
        },
        [&](ExternalApplications::Application application,
            const QString &path) {
          settings.setValue(key(application), path);
          settings.sync();
        }};
    int launches        = 0;
    bool launchSucceeds = true;
    ExternalApplications::Launch received;
    ExternalApplications applications(
        directory.path(), QCoreApplication::applicationFilePath(), adapter,
        [&](const ExternalApplications::Launch &request, QString *error) {
          ++launches;
          received = request;
          if (!launchSucceeds && error) *error = "fixture launch failed";
          return launchSucceeds;
        });
    QAction ztoryc, ot3d;
    applications.bindAction(ExternalApplications::Ztoryc, &ztoryc);
    applications.bindAction(ExternalApplications::OT3D, &ot3d);
    require(!ztoryc.isEnabled() && !ot3d.isEnabled(),
            "empty paths enabled a launch command");
    QString error;
    require(!applications.launch(ExternalApplications::Ztoryc, &error) &&
                !error.isEmpty() && launches == 0,
            "unconfigured application was launched");
    std::unique_ptr<QWidget> widget(
        createExternalApplicationsSettings(applications));
    auto *ztorycPath = widget->findChild<QLineEdit *>("ztorycApplicationPath");
    auto *ot3dPath   = widget->findChild<QLineEdit *>("ot3dApplicationPath");
    require(ztorycPath && ot3dPath && ztorycPath->text().isEmpty() &&
                ot3dPath->text().isEmpty(),
            "Addons did not expose both explicit paths");

    const QString relative = "Related Application With Spaces/program.exe";
    const QString path     = directory.path() + "/" + relative;
    executable(path);
    ztorycPath->setText(relative);
    require(QMetaObject::invokeMethod(ztorycPath, "editingFinished",
                                      Qt::DirectConnection),
            "Addons path could not be committed");
    require(ztoryc.isEnabled() && !ot3d.isEnabled() &&
                settings.value("ztoryc") == relative,
            "Addons did not save the path and update the matching action");
    RestoreEnvironment restore;
    qputenv("QT_PLUGIN_PATH", "fixture-host-plugins");
    qputenv("TOONZROOT", "fixture-host-stuff");
    require(applications.launch(ExternalApplications::Ztoryc, &error) &&
                error.isEmpty() && launches == 1,
            "configured application did not launch");
    require(received.target.executable == QFileInfo(path).canonicalFilePath() &&
                received.target.workingDirectory ==
                    QFileInfo(path).absolutePath() &&
                received.arguments.isEmpty(),
            "launch changed executable, working directory or passed "
            "scene/shell arguments");
    require(
        !received.environment.contains("QT_PLUGIN_PATH") &&
            !received.environment.contains("TOONZROOT") &&
            qgetenv("QT_PLUGIN_PATH") == "fixture-host-plugins" &&
            qgetenv("TOONZROOT") == "fixture-host-stuff",
        "child inherited host paths or launch changed the host environment");
    launchSucceeds = false;
    require(!applications.launch(ExternalApplications::Ztoryc, &error) &&
                error == "fixture launch failed",
            "launch failure was hidden");

    require(QFile::remove(path), "fixture executable could not be removed");
    require(!applications.launch(ExternalApplications::Ztoryc, &error) &&
                !ztoryc.isEnabled() && launches == 2,
            "removed executable was launched or remained enabled");
    executable(path);
    require(!ztoryc.isEnabled(),
            "application discovery occurred without an explicit refresh");
    applications.refresh();
    require(ztoryc.isEnabled(),
            "menu refresh did not revalidate the configured file");
    applications.setConfiguredPath(ExternalApplications::OT3D,
                                   directory.path());
    require(!ot3d.isEnabled(),
            "ordinary directory was accepted as an executable");
    applications.setConfiguredPath(ExternalApplications::OT3D,
                                   QCoreApplication::applicationFilePath());
    require(!ot3d.isEnabled(), "launcher accepted its own executable");
    applications.setConfiguredPath(ExternalApplications::Ztoryc, "");
    require(!ztoryc.isEnabled() && ztorycPath->text().isEmpty(),
            "cleared path did not disable launch");

#ifdef Q_OS_MAC
    const QString bundle = directory.path() + "/Related.app";
    executable(bundle + "/Contents/MacOS/Related");
    QSettings info(bundle + "/Contents/Info.plist", QSettings::NativeFormat);
    info.setValue("CFBundleExecutable", "Related");
    info.sync();
    applications.setConfiguredPath(ExternalApplications::OT3D, bundle);
    require(ot3d.isEnabled(), "macOS application bundle was not resolved");
#endif
    // Exercise the real QProcess failure path with an invalid executable.
    settings.setValue("ztoryc", path);
    ExternalApplications native(
        directory.path(), QCoreApplication::applicationFilePath(), adapter);
    require(!native.launch(ExternalApplications::Ztoryc, &error) &&
                !error.isEmpty(),
            "native process launch failure was not reported");
    std::cout << "externalapplications: PASS\n";
    return 0;
  } catch (const std::exception &error) {
    std::cerr << "externalapplications: FAIL: " << error.what() << '\n';
    return 1;
  }
}
