#include "plugin_parameter_storage.h"

#ifdef BUILD_RELIABILITY_TESTS
#include "tests/plugin_storage_lifetime.h"
#endif

const char* PluginParameterStorage::copyString(const char* source) {
  auto string        = std::make_unique<std::string>(source ? source : "");
  const char* result = string->c_str();
  m_strings.push_back(std::move(string));
  return result;
}

toonz_param_base_t_ PluginParameterStorage::copyBase(
    const toonz_param_base_t_& source) {
  auto result  = source;
  result.label = copyString(source.label);
  return result;
}

toonz_param_desc_t PluginParameterStorage::copyDescriptor(
    const toonz_param_desc_t& source) {
  auto result = source;
  result.base = copyBase(source.base);
  result.key  = copyString(source.key);
  result.note = copyString(source.note);
  if (source.traits_tag == TOONZ_PARAM_TYPE_ENUM) {
    auto values = std::make_unique<const char*[]>(source.traits.e.enums);
    for (int index = 0; index < source.traits.e.enums; ++index)
      values[index] = copyString(source.traits.e.array[index]);
    result.traits.e.array = values.get();
    m_enums.push_back(std::move(values));
  } else if (source.traits_tag == TOONZ_PARAM_TYPE_SPECTRUM) {
    auto values = std::make_unique<SpectrumPoint[]>(source.traits.g.points);
    for (int index = 0; index < source.traits.g.points; ++index)
      values[index] = source.traits.g.array[index];
    result.traits.g.array = values.get();
    m_spectra.push_back(std::move(values));
  } else if (source.traits_tag == TOONZ_PARAM_TYPE_STRING) {
    result.traits.s.def = copyString(source.traits.s.def);
  }
  return result;
}

toonz_param_group_t PluginParameterStorage::copyGroup(
    const toonz_param_group_t& source) {
  auto result      = source;
  result.base      = copyBase(source.base);
  auto descriptors = std::make_unique<toonz_param_desc_t[]>(source.num);
  for (int index = 0; index < source.num; ++index)
    descriptors[index] = copyDescriptor(source.array[index]);
  result.array = descriptors.get();
  m_descriptors.push_back(std::move(descriptors));
  return result;
}

PluginParameterStorage::PluginParameterStorage(int count,
                                               const toonz_param_page_t* source)
    : m_pages(count) {
  for (int index = 0; index < count; ++index) {
    auto& page  = m_pages[index];
    page.base   = copyBase(source[index].base);
    page.num    = source[index].num;
    auto groups = std::make_unique<toonz_param_group_t[]>(page.num);
    for (int group = 0; group < page.num; ++group)
      groups[group] = copyGroup(source[index].array[group]);
    page.array = groups.get();
    m_groups.push_back(std::move(groups));
#ifdef BUILD_RELIABILITY_TESTS
    checkpointPluginStorageCopyForTest();
#endif
  }
}
