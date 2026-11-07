#include "tenv.h"
#include "tfx.h"
#include "tfxparam.h"
#include "tnotanimatableparam.h"
#include "tspectrumparam.h"
#include "toonzqt/pluginloader.h"
#include "toonzqt/pluginmanager.h"
#include "pluginhost.h"

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

void checkReplacedParameters(TFx& effect) {
  TIntEnumParamP choice(effect.getParams()->getParam("fixture_enum"));
  TSpectrumParamP spectrum(effect.getParams()->getParam("fixture_spectrum"));
  require(choice && choice->getValue() == 1 && choice->getItemCount() == 2,
          "Repeated plugin setup lost the final enum descriptor");
  int item = -1;
  std::string caption;
  choice->getItem(1, item, caption);
  require(item == 1 && caption == "Second",
          "Plugin enum labels did not survive setup storage destruction");
  require(spectrum && spectrum->getKeyCount() == 2,
          "Repeated plugin setup lost its spectrum descriptor");
  const auto value = spectrum->getValue(0);
  require(value.getKey(0).second == TPixel32::Red &&
              value.getKey(1).second == TPixel32::Blue,
          "Plugin spectrum colors did not survive setup storage destruction");
}
}  // namespace

class PluginHostLifetimeTest {
public:
  static void check(const PluginDeclaration& declaration) {
    PluginInformation* information = declaration.pi_;
    require(
        information && information->handler_ && information->handler_->destroy,
        "Plugin lifetime fixture lacks its original destroy callback");
    using DestroyCallback = decltype(information->handler_->destroy);
    struct RestoreCallback {
      PluginInformation* information;
      DestroyCallback callback;
      ~RestoreCallback() { information->handler_->destroy = callback; }
    } restore{information, information->handler_->destroy};
    const int references = information->ref_count_;
    for (bool enabled : {true, false}) {
      information->handler_->destroy = enabled ? restore.callback : nullptr;
      for (int cycle = 0; cycle < 8; ++cycle) {
        {
          std::unique_ptr<TFx> effect(
              PluginLoader::create_host("_plg_fixture_single_probe"));
          require(effect && information->ref_count_ == references + 1,
                  "Plugin host did not retain its metadata exactly once");
          checkReplacedParameters(*effect);
        }
        require(information->ref_count_ == references,
                "Plugin host destruction retained a metadata reference");
      }
    }
  }
};

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
      checkReplacedParameters(*effect);
      const auto* declaration =
          static_cast<const PluginDeclaration*>(effect->getDeclaration());
      effect.reset();
      PluginHostLifetimeTest::check(*declaration);
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
