#include "pluginmanagerpopup.h"
#include "toonzqt/pluginmanager.h"

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLockFile>
#include <QProcess>
#include <QTemporaryDir>
#include <QTreeWidget>

#include <iostream>

namespace {
int failures = 0;
void check(bool condition, const char *message) {
  if (condition) return;
  std::cerr << "FAIL: " << message << std::endl;
  ++failures;
}
void write(const QString &path, const QByteArray &bytes) {
  QDir().mkpath(QFileInfo(path).absolutePath());
  QFile file(path);
  check(file.open(QIODevice::WriteOnly), "open fixture");
  check(file.write(bytes) == bytes.size(), "write fixture");
}
QByteArray read(const QString &path) {
  QFile file(path);
  check(file.open(QIODevice::ReadOnly), "read fixture");
  return file.readAll();
}
}  // namespace

int main(int argc, char **argv) {
  QApplication app(argc, argv);
  if (app.arguments().value(1) == "--inspect-policy") {
    PluginManager child(app.arguments().value(2), app.arguments().value(3));
    child.prepareStartup();
    return child.startupFiles().size() == app.arguments().value(4).toInt() ? 0
                                                                           : 1;
  }
  qunsetenv("GROUNDZERO_NO_PLUGINS");
  qunsetenv("GROUNDZERO_FX_PLUGIN_POLICY");
  QTemporaryDir temp;
  check(temp.isValid(), "temporary fixture directory");
  const QString root   = temp.filePath("stuff/plugins");
  const QString config = temp.filePath("stuff/config/plugin-blocklist.txt");
  const QString nested =
      QString::fromUtf8("Package/\xe7\x89\xb9\xe6\xae\x8a.plugin");
  // Invalid binaries are intentional: discovery and UI must not load/probe
  // them.
  write(root + "/Blur.plugin", "not a binary");
  write(root + "/" + nested, "not a binary either");
  write(root + "/Package/runtime.dll", "dependency");
  PluginManager manager(root, config);
  check(manager.settings().error.isEmpty(),
        "missing configuration preserves defaults");
  check(manager.installedFiles().size() == 2,
        "recursive discovery excludes dependencies");
  QString error;
  check(manager.setEnabled(nested, false, &error),
        "disable nested Unicode plugin");
  check(manager.setBypassAll(true, &error), "enable global bypass");
  check(manager.settings().blocked.size() == 1,
        "bypass preserves individual selection");
  PluginManager bypassed(root, config);
  check(bypassed.startupFiles().isEmpty(), "bypass prevents all startup loads");
  check(manager.setBypassAll(false, &error), "disable global bypass");
  check(manager.settings().blocked.size() == 1, "unbypass retains blocklist");
  PluginManager editor(root, config), renderer(root, config);
  const auto editorFiles = editor.startupFiles();
  check(editorFiles.size() == 1 && editorFiles.first().endsWith("Blur.plugin"),
        "blocklist filters before the loader receives paths");
  check(renderer.startupFiles() == editorFiles,
        "editor and renderer share policy");
  check(editor.startupFiles().isEmpty(),
        "session cannot accidentally reload libraries");
  check(manager.setEnabled("Blur.plugin", false, &error),
        "change next startup");
  check(editor.session().settings.blocked.size() == 1,
        "editing configuration leaves current session unchanged");
  check(QFileInfo::exists(root + "/Blur.plugin") &&
            QFileInfo::exists(root + "/" + nested),
        "disable leaves files in place");

  PluginManager parentPolicy(root, config);
  parentPolicy.prepareStartup();
  check(parentPolicy.startupFiles().isEmpty(),
        "freeze disabled policy for current process");
  check(manager.setEnabled("Blur.plugin", true, &error),
        "save a next-session enable");
  QProcess child;
  child.start(QCoreApplication::applicationFilePath(),
              {"--inspect-policy", root, config, "0"});
  check(child.waitForFinished(10000) &&
            child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
        "real child renderer inherits frozen parent policy");
  qunsetenv("GROUNDZERO_FX_PLUGIN_POLICY");
  child.start(QCoreApplication::applicationFilePath(),
              {"--inspect-policy", root, config, "1"});
  check(child.waitForFinished(10000) &&
            child.exitStatus() == QProcess::NormalExit && child.exitCode() == 0,
        "independent renderer reads next-session settings");

  write(config,
        "\xef\xbb\xbf# manual comment\r\n./Blur.plugin\r\nMissing.plugin\r\n");
  check(manager.setEnabled("Blur.plugin", true, &error),
        "edit BOM/CRLF configuration");
  check(read(config).contains("# manual comment") &&
            read(config).contains("Missing.plugin"),
        "edits preserve comments and temporarily missing entries");
  check(!manager.setEnabled("../outside.plugin", false, &error),
        "reject escaping paths");
  {
    QLockFile lock(config + ".lock");
    check(lock.tryLock(), "hold concurrent edit lock");
    check(!manager.setBypassAll(true, &error),
          "report conflicting edit without overwrite");
  }
  write(config, "../outside.plugin\n");
  PluginManager malformed(root, config);
  check(!malformed.settings().error.isEmpty(),
        "report malformed configuration");
  check(malformed.startupFiles().isEmpty(), "malformed policy fails closed");
  check(!manager.setBypassAll(true, &error),
        "do not overwrite malformed policy");
  write(config, "# reset\n");

  const QString package = temp.filePath("downloads/Effects");
  write(package + "/Glow.plugin", "library");
  write(package + "/data/preset.txt", "preset");
  write(package + "/runtime.dll", "runtime");
  check(manager.install(package, &error), "install complete package");
  check(read(root + "/Effects/data/preset.txt") == "preset" &&
            read(root + "/Effects/runtime.dll") == "runtime",
        "preserve package resources");
  check(!manager.install(package, &error), "refuse installation collision");
  check(read(root + "/Effects/Glow.plugin") == "library",
        "collision preserves originals");
  check(!manager.install(temp.path(), &error),
        "reject source containing destination");
  const QString empty = temp.filePath("downloads/Empty");
  write(empty + "/readme.txt", "no plugin");
  check(!manager.install(empty, &error), "reject package without plugins");
  check(!QFileInfo::exists(root + "/Empty"),
        "failed install publishes nothing");
  const QString library = temp.filePath("downloads/Single.plugin");
  write(library, "single library");
  check(manager.install(library, &error), "install single library");
  check(manager.settings().blocked.isEmpty(),
        "new libraries enabled by default");

  manager.startupFiles();
  manager.recordLoadResult(root + "/Blur.plugin", true, QString());
  manager.recordLoadResult(root + "/Effects/Glow.plugin", false,
                           "Missing dependency");
  PluginSettingsWidget preferences(&manager);
  PluginManagerPopup popup(&manager);
  auto *prefBypass  = preferences.findChild<QCheckBox *>("BypassAllPlugins");
  auto *popupBypass = popup.findChild<QCheckBox *>("BypassAllPlugins");
  auto *tree        = popup.findChild<QTreeWidget *>("PluginLibraries");
  check(prefBypass && popupBypass && tree,
        "manager and preferences controls exist");
  check(tree->topLevelItemCount() == 4,
        "manager lists libraries without executing them");
  prefBypass->setChecked(true);
  app.processEvents();
  check(popupBypass->isChecked(), "preferences bypass synchronizes to manager");
  popupBypass->setChecked(false);
  check(!prefBypass->isChecked(), "manager bypass synchronizes to preferences");
  app.processEvents();
  for (int i = 0; i < tree->topLevelItemCount(); ++i) {
    auto *item = tree->topLevelItem(i);
    if (item->text(0) == "Blur.plugin") {
      check(item->text(1) == "Loaded", "loaded session status");
      item->setCheckState(0, Qt::Unchecked);
    }
    if (item->text(0) == "Effects/Glow.plugin")
      check(
          item->text(1) == "Failed" && item->toolTip(1) == "Missing dependency",
          "failed load retains diagnostic");
  }
  app.processEvents();
  check(manager.settings().blocked.size() == 1,
        "checkbox persists per-library disable");
  check(manager.session().results.value("Blur.plugin").loaded,
        "disable does not unload current library");

  char executable[]      = "groundzero";
  char flag[]            = "--no-plugins";
  char scene[]           = "scene.tnz";
  char *arguments[]      = {executable, flag, scene, flag, nullptr};
  int count              = 4;
  const QByteArray saved = read(config);
  PluginManager::takeLaunchArguments(count, arguments);
  check(count == 2 && arguments[1] == scene && !arguments[2],
        "recovery switch consumed without losing scene arguments");
  PluginManager recovery(root, config);
  check(recovery.startupFiles().isEmpty() && recovery.session().launchBypass,
        "launch recovery overrides configuration");
  check(read(config) == saved,
        "launch recovery does not persist preference changes");
  // A child tcomposer inherits the recovery environment.
  PluginManager recoveryRenderer(root, config);
  check(recoveryRenderer.startupFiles().isEmpty(),
        "renderer inherits recovery bypass");
  qunsetenv("GROUNDZERO_NO_PLUGINS");
  if (!failures) std::cout << "Plugin Manager regressions passed.\n";
  return failures ? 1 : 0;
}
