#include "toonzqt/pluginmanager.h"
#include "tenv.h"

PluginManager *PluginManager::instance() {
  // Groundzero and tcomposer resolve the same stuff directory before loading
  // FX.
  static PluginManager manager(
      (TEnv::getStuffDir() + "plugins").getQString(),
      (TEnv::getStuffDir() + "config" + "plugin-blocklist.txt").getQString());
  return &manager;
}
