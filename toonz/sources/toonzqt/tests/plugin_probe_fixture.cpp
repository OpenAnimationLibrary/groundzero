#include "toonz_plugin.h"
#include "toonz_hostif.h"

#include <cstdlib>
#include <cstring>

namespace {
toonz_nodal_rasterfx_handler_t handler = {};
toonz_plugin_probe_t entries[2]        = {};
toonz_plugin_probe_list_t list         = {};

int destroy(toonz_node_handle_t) { return TOONZ_OK; }
}  // namespace

extern "C" TOONZ_EXPORT const toonz_plugin_probe_list_t* toonz_plugin_probe() {
  handler.ver           = {1, 0};
  handler.destroy       = destroy;
  entries[0].ver        = {1, 0};
  entries[0].plugin_ver = {1, 0};
  entries[0].name       = "Single probe fixture";
  entries[0].vendor     = "Test";
  entries[0].id         = "fixture_single_probe";
  entries[0].note       = "";
  entries[0].helpurl    = "";
  entries[0].clss       = TOONZ_PLUGIN_CLASS_POSTPROCESS_SLAB;
  entries[0].handler    = &handler;
  const char* mode      = std::getenv("GROUNDZERO_TEST_PLUGIN_PROBE_MODE");
  const bool empty      = mode && std::strcmp(mode, "empty") == 0;
  list                  = {{1, 0}, entries, empty ? entries : entries + 1};
  return &list;
}

extern "C" TOONZ_EXPORT int toonz_plugin_init(toonz_host_interface_t* host) {
  return host ? TOONZ_OK : TOONZ_ERROR_NULL;
}
