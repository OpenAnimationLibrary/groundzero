#include "tenv.h"
#include "tfx.h"
#include "toonzqt/pluginloader.h"
#include "toonzqt/pluginmanager.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>

#include <iostream>
#include <memory>
#include <stdexcept>

namespace {
void require(bool condition, const char* message) {
  if (!condition) throw std::runtime_error(message);
}
}  // namespace

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  try {
    const QStringList arguments = app.arguments();
    require(arguments.size() == 3,
            "Expected plugin fixture path and probe mode");
    const QFileInfo fixture(arguments[1]);
    QDir stuff(fixture.absolutePath());
    require(fixture.isFile() && stuff.dirName() == "plugins" && stuff.cdUp(),
            "Plugin fixture must be in its private stuff/plugins directory");
    require(arguments[2] == "single" || arguments[2] == "empty",
            "Expected single or empty probe mode");
    qunsetenv("GROUNDZERO_NO_PLUGINS");
    qunsetenv("GROUNDZERO_FX_PLUGIN_POLICY");
    qputenv("GROUNDZERO_TEST_PLUGIN_PROBE_MODE", arguments[2].toUtf8());
    TEnv::setStuffDir(TFilePath(stuff.absolutePath()));

    QElapsedTimer timer;
    timer.start();
    while (!PluginLoader::load_entries(stuff.absolutePath().toStdString())) {
      require(timer.elapsed() < 10000, "Plugin discovery did not finish");
      app.processEvents();
    }
    app.processEvents();

    const auto session = PluginManager::instance()->session();
    require(session.results.contains(fixture.fileName()),
            "Plugin fixture was not discovered");
    const auto loaded = session.results.value(fixture.fileName());
    std::unique_ptr<TFx> effect(
        PluginLoader::create_host("_plg_fixture_single_probe"));
    if (arguments[2] == "single") {
      require(loaded.loaded && loaded.error.isEmpty() && effect,
              "A function probe containing one effect was rejected");
    } else {
      require(!loaded.loaded && !loaded.error.isEmpty() && !effect,
              "An empty function probe was accepted");
    }
    std::cout << "Native plugin function probe: PASS\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
