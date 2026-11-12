#include "toonz_plugin.h"
#include "toonz_hostif.h"
#include "toonz_params.h"

#include <cstdlib>
#include <cstring>

namespace {
toonz_nodal_rasterfx_handler_t handler = {};
toonz_plugin_probe_t entries[2]        = {};
toonz_plugin_probe_list_t list         = {};
toonz_host_interface_t* hostInterface  = nullptr;

int destroy(toonz_node_handle_t) { return TOONZ_OK; }

int setup(toonz_node_handle_t node) {
  void* queried = nullptr;
  if (hostInterface->query_interface(TOONZ_UUID_SETUP, &queried) != TOONZ_OK)
    return TOONZ_ERROR_UNKNOWN;
  auto* api            = static_cast<toonz_setup_interface_t*>(queried);
  const char* labels[] = {"First", "Second"};
  toonz_param_spectrum_t colors[]  = {{0, 1, 0, 0, 1}, {1, 0, 0, 1, 1}};
  toonz_param_desc_t parameters[2] = {};
  parameters[0].base        = {{1, 0}, TOONZ_PARAM_DESC_TYPE_PARAM, "Choice"};
  parameters[0].key         = "fixture_enum";
  parameters[0].traits_tag  = TOONZ_PARAM_TYPE_ENUM;
  parameters[0].traits.e    = {0, 2, labels, 0};
  parameters[1].base        = {{1, 0}, TOONZ_PARAM_DESC_TYPE_PARAM, "Colors"};
  parameters[1].key         = "fixture_spectrum";
  parameters[1].traits_tag  = TOONZ_PARAM_TYPE_SPECTRUM;
  parameters[1].traits.g    = {0, 2, colors, 0};
  toonz_param_group_t group = {
      {{1, 0}, TOONZ_PARAM_DESC_TYPE_GROUP, "Fixture"}, 2, parameters};
  toonz_param_page_t page = {
      {{1, 0}, TOONZ_PARAM_DESC_TYPE_PAGE, "Fixture"}, 1, &group};
  int result = TOONZ_OK;
  for (int cycle = 0; cycle < 16 && result == TOONZ_OK; ++cycle) {
    parameters[0].traits.e.def = cycle % 2;
    result                     = api->set_parameter_pages(node, 1, &page);
  }
  hostInterface->release_interface(api);
  return result;
}
}  // namespace

extern "C" TOONZ_EXPORT const toonz_plugin_probe_list_t* toonz_plugin_probe() {
  handler.ver           = {1, 0};
  handler.destroy       = destroy;
  handler.setup         = setup;
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
  hostInterface = host;
  return host ? TOONZ_OK : TOONZ_ERROR_NULL;
}
