#pragma once

#include "toonz_params.h"

#include <memory>
#include <string>
#include <vector>

// Owns one validated SDK tree; raw descriptor links remain valid for its lease.
class PluginParameterStorage final {
  using SpectrumPoint = toonz_param_traits_spectrum_t::valuetype;
  std::vector<toonz_param_page_t> m_pages;
  std::vector<std::unique_ptr<toonz_param_group_t[]>> m_groups;
  std::vector<std::unique_ptr<toonz_param_desc_t[]>> m_descriptors;
  std::vector<std::unique_ptr<const char*[]>> m_enums;
  std::vector<std::unique_ptr<SpectrumPoint[]>> m_spectra;
  std::vector<std::unique_ptr<std::string>> m_strings;

  const char* copyString(const char* source);
  toonz_param_base_t_ copyBase(const toonz_param_base_t_& source);
  toonz_param_desc_t copyDescriptor(const toonz_param_desc_t& source);
  toonz_param_group_t copyGroup(const toonz_param_group_t& source);

public:
  PluginParameterStorage(int count, const toonz_param_page_t* source);
  const std::vector<toonz_param_page_t>& pages() const { return m_pages; }
};
